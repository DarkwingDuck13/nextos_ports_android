/*
 * BADLAND (Cocos2d-x + FMOD Studio) -> aarch64 Linux so-loader.
 *
 * Primeiro bring-up: boot seguro, assets SD, ETC2 -> RGBA no hook de GL,
 * controle por touch fallback. Depois de abrir o jogo, otimizar textura/audio.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <GLES2/gl2.h>
#include <SDL2/SDL.h>

#include "error.h"
#include "imports.h"
#include "jni_shim.h"
#include "so_util.h"
#include "util.h"

typedef int jint;
typedef unsigned char jboolean;

#define FMOD_SO "libfmod.so"
#define FMODSTUDIO_SO "libfmodstudio.so"
#define GAME_SO "libbadland.so"

#define FMOD_HEAP_MB 64
#define FMODSTUDIO_HEAP_MB 64
#define GAME_HEAP_MB 320

#define AKEYCODE_BACK 4
#define AKEYCODE_DPAD_UP 19
#define AKEYCODE_DPAD_DOWN 20
#define AKEYCODE_DPAD_LEFT 21
#define AKEYCODE_DPAD_RIGHT 22
#define AKEYCODE_DPAD_CENTER 23
#define AKEYCODE_BUTTON_A 96
#define AKEYCODE_BUTTON_B 97
#define AKEYCODE_BUTTON_X 99
#define AKEYCODE_BUTTON_Y 100
#define AKEYCODE_BUTTON_L1 102
#define AKEYCODE_BUTTON_R1 103
#define AKEYCODE_BUTTON_START 108
#define AKEYCODE_BUTTON_SELECT 109

#define BADLAND_PAD_UP 0
#define BADLAND_PAD_DOWN 1
#define BADLAND_PAD_LEFT 2
#define BADLAND_PAD_RIGHT 3
#define BADLAND_PAD_A 4
#define BADLAND_PAD_B 5
#define BADLAND_PAD_X 6
#define BADLAND_PAD_Y 7
#define BADLAND_PAD_L2 8
#define BADLAND_PAD_R2 9
#define BADLAND_PAD_LEFT_ANALOG 10
#define BADLAND_PAD_MENU 12
#define BADLAND_PAD_L1 13
#define BADLAND_PAD_R1 14

#define BADLAND_PAD_ACTION_DOWN 0
#define BADLAND_PAD_ACTION_UP 1
#define BADLAND_PAD_ACTION_ANALOG 2

#define BADLAND_OFF_PVR_V2 0x98b1fc
#define BADLAND_OFF_PVR_V3 0x98ba70
#define BADLAND_OFF_PVR_CREATE_GL 0x98c0fc
#define BADLAND_GOT_PVR_V2 0x12b5d58
#define BADLAND_GOT_PVR_V3 0x12b5d60
#define BADLAND_GOT_PVR_CREATE_GL 0x12b5d68
#define BADLAND_VT_CCFILEUTILSANDROID_GETFILEDATA 0x121df80
#define BADLAND_OFF_CONFIG_STOP_ALL_SOUNDS 0xa5fe04
#define BADLAND_OFF_CONFIG_UPDATE_FMOD 0xa5fe2c
#define BADLAND_OFF_CONFIG_SET_NORMAL_REVERB 0xa5fe9c
#define BADLAND_OFF_CONFIG_SET_TIMESCAPE_REVERB 0xa5fec4
#define BADLAND_OFF_CONFIG_IS_FMOD_READY 0xa5feec
#define BADLAND_OFF_AUDIO_INITIALIZED 0xa5ff10
#define BADLAND_OFF_EVENT_SOUND_SET_VISIBILITY 0xe90020
#define BADLAND_OFF_EVENT_SOUND_SET_ACTIVE 0xe90108
#define BADLAND_OFF_EVENT_SOUND_INIT_SOUND_EVENT 0xe9015c
#define BADLAND_OFF_EVENT_SOUND_PAUSE_SOUND 0xe920b8
#define BADLAND_OFF_AUDIO_GET_AUDIO_EVENT_DESC 0xe92194
#define BADLAND_OFF_AUDIO_SUSPEND 0xf614f8
#define BADLAND_OFF_AUDIO_RESUME 0xf61598
#define BADLAND_OFF_AUDIO_PLAY_MUSIC 0xf61e98
#define BADLAND_OFF_AUDIO_SET_SOUND_POSITION 0xf623f8
#define BADLAND_OFF_AUDIO_SET_GLOBAL_VOLUME 0xf62490
#define BADLAND_OFF_AUDIO_STOP_ALL_SOUNDS 0xf623bc
#define BADLAND_OFF_AUDIO_PLAY_SOUND_CATEGORY 0xf62544
#define BADLAND_OFF_AUDIO_PLAY_SOUND_IMPL 0xf6267c
#define BADLAND_OFF_AUDIO_INIT_AUDIO_EVENT 0xf628dc
#define BADLAND_OFF_AUDIO_PLAY_SOUND 0xf62a40
#define BADLAND_OFF_AUDIO_PLAY_SOUND_CUSTOM 0xf62b10
#define BADLAND_OFF_AUDIO_STOP_MUSIC 0xf62bbc
#define BADLAND_OFF_AUDIO_PLAY_AMBIENT 0xf62be4
#define BADLAND_OFF_AUDIO_UNINIT_BANK 0xf62c90
#define BADLAND_OFF_AUDIO_SET_LISTENER_POSITION 0xf62cf0
#define BADLAND_OFF_AUDIO_UPDATE 0xf62ee0
#define BADLAND_OFF_AUDIO_UNINIT 0xf62f60

__attribute__((used, aligned(16))) _Thread_local char g_bionic_guard_pad[256];

static void *g_env;
static SDL_GameController *g_controller;
static char g_watchdog_path[PATH_MAX];
static char g_last_pvr_path[PATH_MAX];
static time_t g_watchdog_last_write;
static int g_width = 1280;
static int g_height = 720;
static int g_touch_down;
static int g_enable_fmod;
static int g_enable_resume;
static int g_input_log;
static int g_input_selftest;

static jint (*JNI_OnLoad_fn)(void *vm, void *reserved);
static jint (*fmod_JNI_OnLoad_fn)(void *vm, void *reserved);
static void (*nativeSetApkPath)(void *env, void *thiz, void *apkPath);
static void (*nativeInit)(void *env, void *thiz, int arg0, int w, int h);
static void (*nativeRender)(void *env, void *thiz);
static void (*nativeOnStart)(void *env, void *thiz);
static void (*nativeOnStop)(void *env, void *thiz);
static void (*nativeOnPause)(void *env, void *thiz);
static void (*nativeOnResume)(void *env, void *thiz);
static void (*nativeKeyDown)(void *env, void *thiz, int keyCode);
static void (*nativePadAction)(void *env, void *thiz, int deviceId, int action,
                               int button, float x, float y);
static void (*nativeTouchesBegin)(void *env, void *thiz, int id, float x,
                                  float y);
static void (*nativeTouchesEnd)(void *env, void *thiz, int id, float x,
                                float y);
static void (*nativeTouchesMove)(void *env, void *thiz, int ids, void *xs,
                                 void *ys);
static void (*nativeInitializeAssetManager)(void *env, void *thiz,
                                            void *assetManager);
static void (*nativeInitFmod)(void *env, void *thiz, int low_latency);
static void (*nativeReportAudioProperties)(void *env, void *thiz,
                                           int sampleRate, int framesPerBuffer);
static void (*nativeSetPreferredFmodOutput)(void *env, void *thiz, int output);
static void (*nativeSetForceAudioTrackOutput)(void *env, void *thiz,
                                              jboolean force);
static void (*nativeSetAudioPaused)(void *env, void *thiz, jboolean paused);

static uintptr_t g_game_base;
static int (*real_pvr_v2)(void *self, unsigned char *data, unsigned int size);
static int (*real_pvr_v3)(void *self, unsigned char *data, unsigned int size);
static int (*real_pvr_create_gl)(void *self);
static unsigned char *(*real_ccfile_get_data)(void *self, void *filename,
                                              const char *mode, size_t *size);
static void *(*game_new_array)(size_t size);

typedef struct {
  void *event;
  void *channel;
} BadlandAudioRet;

static uint32_t rd32le(const unsigned char *p) {
  return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t rd64le(const unsigned char *p) {
  return ((uint64_t)rd32le(p)) | ((uint64_t)rd32le(p + 4) << 32);
}

static void wr32le(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
  p[2] = (unsigned char)((v >> 16) & 0xff);
  p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void wr64le(unsigned char *p, uint64_t v) {
  wr32le(p, (uint32_t)v);
  wr32le(p + 4, (uint32_t)(v >> 32));
}

static int is_pvr3(const unsigned char *data, unsigned int size) {
  return data && size >= 4 && data[0] == 'P' && data[1] == 'V' &&
         data[2] == 'R' && data[3] == 3;
}

static int badland_path_log_wanted(const char *path) {
  return path && (strstr(path, ".pvr") || strstr(path, "splash"));
}

static const char *ndk_string_c_str(const void *str) {
  const unsigned char *s = (const unsigned char *)str;
  if (!s)
    return NULL;
  if (s[0] & 1)
    return *(const char *const *)(s + 16);
  return (const char *)(s + 1);
}

static int badland_is_world_name(const char *start, size_t len) {
  return (len == 3 && memcmp(start, "day", 3) == 0) ||
         (len == 4 &&
          (memcmp(start, "dawn", 4) == 0 || memcmp(start, "dusk", 4) == 0)) ||
         (len == 5 && memcmp(start, "night", 5) == 0);
}

static int badland_world_from_path(const char *path, char *world,
                                   size_t world_size) {
  if (!path || !world || world_size == 0)
    return 0;
  const char *base = strrchr(path, '/');
  if (!base || base == path)
    return 0;

  const char *start = base;
  while (start > path && start[-1] != '/')
    start--;
  size_t len = (size_t)(base - start);
  if (!badland_is_world_name(start, len))
    return 0;
  if (len >= world_size)
    return 0;
  memcpy(world, start, len);
  world[len] = '\0';
  return 1;
}

static size_t badland_replace_inplace(unsigned char *data, size_t len,
                                      const char *from, const char *to) {
  size_t from_len = strlen(from);
  size_t to_len = strlen(to);
  if (!data || !from_len || to_len > from_len)
    return len;

  size_t off = 0;
  while (off < len) {
    unsigned char *pos = memmem(data + off, len - off, from, from_len);
    if (!pos)
      break;
    size_t idx = (size_t)(pos - data);
    size_t tail = len - idx - from_len;
    memcpy(pos, to, to_len);
    memmove(pos + to_len, pos + from_len, tail + 1);
    len -= from_len - to_len;
    off = idx + to_len;
  }
  return len;
}

static int badland_split_world_asset(const char *path, char *stem,
                                     size_t stem_size, int *is_plist) {
  if (!path || !stem || stem_size == 0 || !is_plist)
    return 0;

  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  size_t len = strlen(base);

  *is_plist = 0;
  if (len > 6 && strcmp(base + len - 6, ".plist") == 0) {
    len -= 6;
    *is_plist = 1;
  } else if (len > 7 && strcmp(base + len - 7, "-hd.pvr") == 0) {
    len -= 7;
  } else if (len > 4 && strcmp(base + len - 4, ".pvr") == 0) {
    len -= 4;
  } else {
    return 0;
  }

  if (len == 0 || len >= stem_size)
    return 0;
  memcpy(stem, base, len);
  stem[len] = '\0';
  return 1;
}

static const char *badland_graphics_relative(const char *path) {
  if (!path)
    return NULL;

  const char *relative = path;
  if (strncmp(relative, "./assets/", 9) == 0)
    relative += 9;
  else if (strncmp(relative, "assets/", 7) == 0)
    relative += 7;

  if (strncmp(relative, "graphics/hd_etc2/", 17) == 0)
    relative += 17;
  else if (strncmp(relative, "graphics/sd_etc2/", 17) == 0)
    relative += 17;
  else if (strncmp(relative, "graphics/hd_common/", 19) == 0)
    relative += 19;
  else if (strncmp(relative, "graphics/sd_common/", 19) == 0)
    relative += 19;
  else if (strncmp(relative, "graphics/hd/", 12) == 0)
    relative += 12;
  else if (strncmp(relative, "graphics/sd/", 12) == 0)
    relative += 12;

  return relative;
}

static int badland_split_graphics_asset(const char *path, char *stem,
                                        size_t stem_size, int *is_plist) {
  const char *relative = badland_graphics_relative(path);
  if (!relative || !stem || stem_size == 0 || !is_plist)
    return 0;

  size_t len = strlen(relative);
  *is_plist = 0;
  if (len > 6 && strcmp(relative + len - 6, ".plist") == 0) {
    len -= 6;
    *is_plist = 1;
  } else if (len > 7 && strcmp(relative + len - 7, "-hd.pvr") == 0) {
    len -= 7;
  } else if (len > 4 && strcmp(relative + len - 4, ".pvr") == 0) {
    len -= 4;
  } else {
    return 0;
  }

  if (len == 0 || len >= stem_size)
    return 0;
  memcpy(stem, relative, len);
  stem[len] = '\0';
  return 1;
}

static int badland_hd_ui_enabled(void) {
  const char *env = getenv("BADLAND_HD_UI");
  return !(env && strcmp(env, "0") == 0);
}

static int badland_is_font_stem(const char *stem) {
  if (!stem || strchr(stem, '/'))
    return 0;
  return strcmp(stem, "font") == 0 || strcmp(stem, "font-cn") == 0 ||
         strcmp(stem, "font-jp") == 0 || strcmp(stem, "font-kr") == 0 ||
         strcmp(stem, "font-ru") == 0;
}

static int badland_is_hd_ui_asset(const char *stem) {
  if (!stem || !*stem)
    return 0;
  return strncmp(stem, "menus/level-pack-", 17) == 0 ||
         strcmp(stem, "menus/level-packs") == 0 ||
         strncmp(stem, "UI Assets/", 10) == 0 ||
         badland_is_font_stem(stem);
}

static int badland_use_hd_global_asset(const char *stem) {
  if (!stem || !*stem)
    return 0;
  static const char *prefixes[] = {
      "obstacles/",
      "particles/",
  };
  for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
    size_t n = strlen(prefixes[i]);
    if (strncmp(stem, prefixes[i], n) == 0)
      return 1;
  }
  return badland_is_hd_ui_asset(stem) || strcmp(stem, "items") == 0 ||
         strcmp(stem, "eyes") == 0 ||
         strcmp(stem, "Avatars") == 0 || strcmp(stem, "Avatars-flappy") == 0 ||
         strcmp(stem, "Avatars-special-android") == 0 ||
         strcmp(stem, "Avatars-special-pumpkin") == 0 ||
         strncmp(stem, "Avatar-wing-", 12) == 0 ||
         strncmp(stem, "Avatar-special-", 15) == 0;
}

static unsigned char *badland_read_file_direct(const char *resolved,
                                               const char *mode,
                                               size_t *out_size) {
  FILE *f = fopen(resolved, mode && mode[0] ? mode : "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long len = ftell(f);
  if (len < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  unsigned char *data = game_new_array ? game_new_array((size_t)len + 1)
                                       : malloc((size_t)len + 1);
  if (!data) {
    fclose(f);
    return NULL;
  }
  size_t got = fread(data, 1, (size_t)len, f);
  fclose(f);
  if (got != (size_t)len) {
    free(data);
    return NULL;
  }
  data[got] = 0;
  if (out_size)
    *out_size = got;
  return data;
}

static unsigned char *badland_read_hd_world_asset(const char *path,
                                                  const char *mode,
                                                  size_t *out_size) {
  const char *env = getenv("BADLAND_HD_SCENERY");
  if (env && strcmp(env, "0") == 0)
    return NULL;

  char world[32];
  if (!badland_world_from_path(path, world, sizeof(world)))
    return NULL;

  char stem[128];
  int want_plist = 0;
  if (!badland_split_world_asset(path, stem, sizeof(stem), &want_plist))
    return NULL;

  char resolved[PATH_MAX];
  const char *families[] = {"hd", "hd_common"};
  int found = 0;
  for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); i++) {
    if (snprintf(resolved, sizeof(resolved), "./assets/graphics/%s/%s/%s%s",
                 families[i], world, stem,
                 want_plist ? ".plist" : "-hd.pvr") >= (int)sizeof(resolved))
      continue;
    if (access(resolved, F_OK) == 0) {
      found = 1;
      break;
    }
  }
  if (!found)
    return NULL;

  size_t got = 0;
  unsigned char *data = badland_read_file_direct(resolved, mode, &got);
  if (!data)
    return NULL;

  if (want_plist) {
    got = badland_replace_inplace(data, got, "-hd.pvr", ".pvr");
  } else {
    snprintf(g_last_pvr_path, sizeof(g_last_pvr_path), "%s", resolved);
  }
  if (out_size)
    *out_size = got;
  fprintf(stderr, "[CCFileUtils] hd scenery \"%s\" -> \"%s\" size=%zu\n",
          path ? path : "(null)", resolved, got);
  return data;
}

static unsigned char *badland_read_hd_global_asset(const char *path,
                                                   const char *mode,
                                                   size_t *out_size) {
  const char *env = getenv("BADLAND_HD_GLOBALS");
  if (env && strcmp(env, "0") == 0)
    return NULL;

  char stem[256];
  int want_plist = 0;
  if (!badland_split_graphics_asset(path, stem, sizeof(stem), &want_plist))
    return NULL;
  if (!badland_use_hd_global_asset(stem))
    return NULL;
  if (badland_is_hd_ui_asset(stem) && !badland_hd_ui_enabled())
    return NULL;

  char resolved[PATH_MAX];
  const char *families[] = {"hd", "hd_common"};
  int found = 0;
  for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); i++) {
    if (snprintf(resolved, sizeof(resolved), "./assets/graphics/%s/%s%s",
                 families[i], stem, want_plist ? ".plist" : "-hd.pvr") >=
        (int)sizeof(resolved))
      continue;
    if (access(resolved, F_OK) == 0) {
      found = 1;
      break;
    }
  }
  if (!found)
    return NULL;

  size_t got = 0;
  unsigned char *data = badland_read_file_direct(resolved, mode, &got);
  if (!data)
    return NULL;

  if (want_plist) {
    got = badland_replace_inplace(data, got, "-hd.pvr", ".pvr");
  } else {
    snprintf(g_last_pvr_path, sizeof(g_last_pvr_path), "%s", resolved);
  }
  if (out_size)
    *out_size = got;
  fprintf(stderr, "[CCFileUtils] hd global \"%s\" -> \"%s\" size=%zu\n",
          path ? path : "(null)", resolved, got);
  return data;
}

static unsigned char *badland_read_hd_ui_text_asset(const char *path,
                                                    const char *mode,
                                                    size_t *out_size) {
  if (!badland_hd_ui_enabled())
    return NULL;

  const char *relative = badland_graphics_relative(path);
  if (!relative)
    return NULL;
  size_t len = strlen(relative);
  if (len <= 4 || strcmp(relative + len - 4, ".fnt") != 0)
    return NULL;
  if (strchr(relative, '/'))
    return NULL;

  char stem[64];
  if (len - 4 >= sizeof(stem))
    return NULL;
  memcpy(stem, relative, len - 4);
  stem[len - 4] = '\0';
  if (!badland_is_font_stem(stem))
    return NULL;

  char resolved[PATH_MAX];
  if (snprintf(resolved, sizeof(resolved), "./assets/graphics/hd_common/%s",
               relative) >= (int)sizeof(resolved))
    return NULL;
  if (access(resolved, F_OK) != 0)
    return NULL;

  size_t got = 0;
  unsigned char *data = badland_read_file_direct(resolved, mode, &got);
  if (!data)
    return NULL;
  if (out_size)
    *out_size = got;
  fprintf(stderr, "[CCFileUtils] hd ui \"%s\" -> \"%s\" size=%zu\n",
          path ? path : "(null)", resolved, got);
  return data;
}

static unsigned char *badland_read_whole_file(const char *path, const char *mode,
                                              size_t *out_size) {
  const char *resolved = resolve_android_path(path);
  FILE *f = fopen(resolved, mode && mode[0] ? mode : "rb");
  if (!f) {
    fprintf(stderr, "[CCFileUtils] fallback open fail \"%s\" -> \"%s\": %s\n",
            path ? path : "(null)", resolved ? resolved : "(null)",
            strerror(errno));
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long len = ftell(f);
  if (len < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  unsigned char *data = game_new_array ? game_new_array((size_t)len + 1)
                                       : malloc((size_t)len + 1);
  if (!data) {
    fclose(f);
    return NULL;
  }
  size_t got = fread(data, 1, (size_t)len, f);
  fclose(f);
  if (got != (size_t)len) {
    free(data);
    return NULL;
  }
  data[got] = 0;
  if (out_size)
    *out_size = got;
  fprintf(stderr, "[CCFileUtils] fallback \"%s\" -> \"%s\" size=%zu\n",
          path ? path : "(null)", resolved ? resolved : "(null)", got);
  return data;
}

static unsigned char *badland_ccfile_get_data(void *self, void *filename,
                                              const char *mode, size_t *size) {
  const char *path = ndk_string_c_str(filename);
  unsigned char *data = NULL;

  data = badland_read_hd_world_asset(path, mode, size);
  if (data)
    return data;

  data = badland_read_hd_global_asset(path, mode, size);
  if (data)
    return data;

  data = badland_read_hd_ui_text_asset(path, mode, size);
  if (data)
    return data;

  if (real_ccfile_get_data)
    data = real_ccfile_get_data(self, filename, mode, size);

  size_t got_size = size ? *size : 0;
  if (badland_path_log_wanted(path))
    fprintf(stderr, "[CCFileUtils] getFileData \"%s\" mode=%s real=%p size=%zu\n",
            path ? path : "(null)", mode ? mode : "(null)", data, got_size);

  if (data && got_size) {
    if (path && strstr(path, ".pvr"))
      snprintf(g_last_pvr_path, sizeof(g_last_pvr_path), "%s", path);
    return data;
  }
  if (!badland_path_log_wanted(path))
    return data;

  data = badland_read_whole_file(path, mode, size);
  if (data && path && strstr(path, ".pvr"))
    snprintf(g_last_pvr_path, sizeof(g_last_pvr_path), "%s", path);
  return data;
}

extern void badland_gl_set_upload_label(const char *label);

static void log_pvr3_header(const char *tag, const unsigned char *data,
                            unsigned int size) {
  if (!is_pvr3(data, size) || size < 52) {
    fprintf(stderr, "[PVR] %s data=%p size=%u magic=%02x %02x %02x %02x\n",
            tag, data, size, data && size > 0 ? data[0] : 0,
            data && size > 1 ? data[1] : 0, data && size > 2 ? data[2] : 0,
            data && size > 3 ? data[3] : 0);
    return;
  }
  uint64_t pf = rd64le(data + 8);
  fprintf(stderr,
          "[PVR] %s PVR3 flags=0x%x pf=0x%016llx %ux%u mip=%u meta=%u "
          "size=%u\n",
          tag, rd32le(data + 4), (unsigned long long)pf, rd32le(data + 28),
          rd32le(data + 24), rd32le(data + 44), rd32le(data + 48), size);
}

extern unsigned char *etc2_decode_rgba(unsigned fmt, int w, int h,
                                       const void *data, int size);

static int pvr3_badland_etc2_glfmt(uint64_t pf) {
  /* BADLAND/Cocos antigo usa 12=ETC2_RGB e 14=ETC2_RGBA.
     A enum PVR moderna usa 22/23/24; deixamos suportado para seguranca. */
  if (pf == 12 || pf == 22)
    return 0x9274; /* GL_COMPRESSED_RGB8_ETC2 */
  if (pf == 14 || pf == 23)
    return 0x9278; /* GL_COMPRESSED_RGBA8_ETC2_EAC */
  if (pf == 24)
    return 0x9276; /* GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2 */
  return 0;
}

static void log_rgba_stats(const char *tag, uint64_t pf, unsigned w,
                           unsigned h, const unsigned char *rgba) {
  if (!rgba || !w || !h)
    return;
  unsigned long long sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0;
  unsigned min_r = 255, min_g = 255, min_b = 255, min_a = 255;
  unsigned max_r = 0, max_g = 0, max_b = 0, max_a = 0;
  size_t total = (size_t)w * h;
  size_t transparent = 0;
  for (size_t i = 0; i < total; i++) {
    unsigned r = rgba[i * 4 + 0], g = rgba[i * 4 + 1];
    unsigned b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
    sum_r += r;
    sum_g += g;
    sum_b += b;
    sum_a += a;
    if (r < min_r)
      min_r = r;
    if (g < min_g)
      min_g = g;
    if (b < min_b)
      min_b = b;
    if (a < min_a)
      min_a = a;
    if (r > max_r)
      max_r = r;
    if (g > max_g)
      max_g = g;
    if (b > max_b)
      max_b = b;
    if (a > max_a)
      max_a = a;
    if (a == 0)
      transparent++;
  }
  fprintf(stderr,
          "[tex] %s pf=0x%llx %ux%u avg=%llu,%llu,%llu,%llu "
          "min=%u,%u,%u,%u max=%u,%u,%u,%u a0=%zu/%zu\n",
          tag, (unsigned long long)pf, w, h, sum_r / total, sum_g / total,
          sum_b / total, sum_a / total, min_r, min_g, min_b, min_a, max_r,
          max_g, max_b, max_a, transparent, total);
}

static void dump_rgba_ppm(const char *tag, unsigned index, uint64_t pf,
                          unsigned w, unsigned h,
                          const unsigned char *rgba) {
  const char *env = getenv("BADLAND_TEXDUMP");
  if (env && strcmp(env, "0") == 0)
    return;
  if (!env && index >= 10)
    return;
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "./logs/texdump-%s-%02u-pf%llx-%ux%u.ppm", tag,
           index, (unsigned long long)pf, w, h);
  FILE *f = fopen(path, "wb");
  if (!f)
    return;
  fprintf(f, "P6\n%u %u\n255\n", w, h);
  for (size_t i = 0, total = (size_t)w * h; i < total; i++)
    fwrite(rgba + i * 4, 1, 3, f);
  fclose(f);
  fprintf(stderr, "[tex] dump %s\n", path);
}

static unsigned char *badland_pvr3_etc2_to_rgba8(const unsigned char *data,
                                                 unsigned int size,
                                                 unsigned int *out_size) {
  if (!is_pvr3(data, size) || size < 52)
    return NULL;
  uint64_t pf = rd64le(data + 8);
  int glfmt = pvr3_badland_etc2_glfmt(pf);
  if (!glfmt)
    return NULL;

  unsigned h = rd32le(data + 24);
  unsigned w = rd32le(data + 28);
  unsigned meta = rd32le(data + 48);
  unsigned data_off = 52u + meta;
  if (!w || !h || data_off > size)
    return NULL;

  unsigned bw = (w + 3u) / 4u;
  unsigned bh = (h + 3u) / 4u;
  unsigned block = (glfmt == 0x9278) ? 16u : 8u;
  size_t compressed_size = (size_t)bw * bh * block;
  if (compressed_size > (size_t)size - data_off)
    return NULL;

  unsigned char *rgba = etc2_decode_rgba((unsigned)glfmt, (int)w, (int)h,
                                         data + data_off,
                                         (int)compressed_size);
  if (!rgba)
    return NULL;

  static unsigned converted_count = 0;
  if (converted_count < 24)
    log_rgba_stats("pvr-etc2->rgba8", pf, w, h, rgba);
  dump_rgba_ppm("pvr", converted_count, pf, w, h, rgba);

  size_t rgba_size = (size_t)w * h * 4u;
  size_t new_size = 52u + rgba_size;
  unsigned char *out = game_new_array ? game_new_array(new_size + 1)
                                      : malloc(new_size + 1);
  if (!out) {
    free(rgba);
    return NULL;
  }
  memset(out, 0, 52);
  memcpy(out, data, 52);
  wr64le(out + 8, 0x0808080861626772ULL); /* RGBA8888 */
  wr32le(out + 24, h);
  wr32le(out + 28, w);
  wr32le(out + 32, 1);
  wr32le(out + 36, 1);
  wr32le(out + 40, 1);
  wr32le(out + 44, 1);
  wr32le(out + 48, 0);
  memcpy(out + 52, rgba, rgba_size);
  out[new_size] = 0;
  free(rgba);
  if (out_size)
    *out_size = (unsigned int)new_size;
  fprintf(stderr,
          "[PVR] converted ETC2 pf=0x%llx gl=0x%x %ux%u size=%u -> "
          "RGBA8888 size=%u\n",
          (unsigned long long)pf, glfmt, w, h, size, (unsigned)new_size);
  converted_count++;
  return out;
}

static int badland_pvr_v2(void *self, unsigned char *data, unsigned int size) {
  static int logged = 0;
  if (logged < 16) {
    log_pvr3_header("v2-skip-check", data, size);
    logged++;
  }
  if (is_pvr3(data, size))
    return 0;
  return real_pvr_v2 ? real_pvr_v2(self, data, size) : 0;
}

static int badland_pvr_v3(void *self, unsigned char *data, unsigned int size) {
  static int logged = 0;
  if (logged < 16) {
    log_pvr3_header("v3", data, size);
    logged++;
  }
  unsigned int converted_size = 0;
  unsigned char *converted =
      badland_pvr3_etc2_to_rgba8(data, size, &converted_size);
  if (converted && converted_size)
    return real_pvr_v3 ? real_pvr_v3(self, converted, converted_size) : 0;
  return real_pvr_v3 ? real_pvr_v3(self, data, size) : 0;
}

static int badland_pvr_create_gl(void *self) {
  static int logged = 0;
  if (logged < 32 && self) {
    unsigned char *p = (unsigned char *)self;
    int mips = *(int *)(p + 280);
    int width = *(int *)(p + 284);
    int height = *(int *)(p + 288);
    void *fmt = *(void **)(p + 304);
    fprintf(stderr, "[PVR] createGL self=%p %dx%d mips=%d fmt=%p\n", self,
            width, height, mips, fmt);
    logged++;
  }
  badland_gl_set_upload_label(g_last_pvr_path);
  int ret = real_pvr_create_gl ? real_pvr_create_gl(self) : 0;
  badland_gl_set_upload_label("");
  if (logged <= 32)
    fprintf(stderr, "[PVR] createGL ret=%d\n", ret);
  return ret;
}

static void patch_game_got(uintptr_t off, void *fn, const char *name) {
  if (!g_game_base || !fn)
    return;
  uintptr_t *slot = (uintptr_t *)(g_game_base + off);
  uintptr_t old = *slot;
  *slot = (uintptr_t)fn;
  fprintf(stderr, "[PVR] GOT %s @+0x%lx %p -> %p\n", name,
          (unsigned long)off, (void *)old, fn);
}

static uintptr_t patch_game_ptr(uintptr_t off, void *fn, const char *name) {
  if (!g_game_base || !fn)
    return 0;
  uintptr_t *slot = (uintptr_t *)(g_game_base + off);
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size > 0) {
    uintptr_t page = (uintptr_t)slot & ~((uintptr_t)page_size - 1);
    mprotect((void *)page, (size_t)page_size, PROT_READ | PROT_WRITE | PROT_EXEC);
  }
  uintptr_t old = *slot;
  *slot = (uintptr_t)fn;
  fprintf(stderr, "[hook] %s @+0x%lx %p -> %p\n", name, (unsigned long)off,
          (void *)old, fn);
  return old;
}

__attribute__((noinline)) static BadlandAudioRet badland_audio_pair_silent(void) {
  BadlandAudioRet ret = {0, 0};
  return ret;
}

__attribute__((noinline)) static void badland_audio_void_silent(void) {}

__attribute__((noinline)) static int badland_audio_int_zero(void) { return 0; }

__attribute__((noinline)) static int badland_audio_int_one(void) { return 1; }

__attribute__((noinline)) static void badland_audio_desc_zero(void) {
#if defined(__aarch64__)
  void *out;
  __asm__ volatile("mov %0, x8" : "=r"(out));
  __asm__ volatile("stp xzr, xzr, [%0]\n"
                   "stp xzr, xzr, [%0, #16]\n"
                   "stp xzr, xzr, [%0, #32]\n"
                   :
                   : "r"(out)
                   : "memory");
#endif
}

static int patch_game_branch_abs(uintptr_t off, void *fn, const char *name) {
#if defined(__aarch64__)
  if (!g_game_base || !fn)
    return 0;
  uintptr_t addr = g_game_base + off;
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
    return 0;
  uintptr_t page = addr & ~((uintptr_t)page_size - 1);
  uintptr_t end = (addr + 16 + (uintptr_t)page_size - 1) &
                  ~((uintptr_t)page_size - 1);
  if (mprotect((void *)page, (size_t)(end - page),
               PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    fprintf(stderr, "[hook] %s @+0x%lx mprotect fail: %s\n", name,
            (unsigned long)off, strerror(errno));
    return 0;
  }

  uint32_t *p = (uint32_t *)addr;
  uint32_t old0 = p[0];
  uint32_t old1 = p[1];
  uintptr_t target = (uintptr_t)fn;
  p[0] = 0x58000050; /* ldr x16, #8 */
  p[1] = 0xd61f0200; /* br x16 */
  memcpy(p + 2, &target, sizeof(target));
  __builtin___clear_cache((char *)addr, (char *)(addr + 16));
  fprintf(stderr, "[hook] branch %s @+0x%lx insn=%08x %08x -> %p\n", name,
          (unsigned long)off, old0, old1, fn);
  return 1;
#else
  (void)off;
  (void)fn;
  (void)name;
  return 0;
#endif
}

static int (*real_fmod_studio_create)(void **system, unsigned flags);
static int (*real_fmod_studio_initialize)(void *system, int max_channels,
                                          unsigned flags, unsigned extra_flags,
                                          void *extra_driver_data);
static int (*real_fmod_studio_load_bank_file)(void *system, const char *path,
                                               unsigned flags, void **bank);
static int (*real_fmod_studio_get_core_system)(void *system,
                                               void **core_system);
static int (*real_fmod_system_set_output)(void *system, int output);
static int (*real_fmod_system_set_output_by_plugin)(void *system,
                                                    unsigned handle);
static int (*real_fmod_system_get_num_plugins)(void *system, int plugin_type,
                                               int *num_plugins);
static int (*real_fmod_system_get_plugin_handle)(void *system, int plugin_type,
                                                 int index,
                                                 unsigned *handle);
static int (*real_fmod_system_get_plugin_info)(void *system, unsigned handle,
                                               int *plugin_type, char *name,
                                               int name_len,
                                               unsigned *version);
static int (*real_fmod_system_set_dsp_buffer_size)(void *system,
                                                   unsigned buffer_length,
                                                   int num_buffers);
static int (*real_fmod_android_jni_init)(void *vm, void *context);

static int spy_fmod_studio_create(void **system, unsigned flags) {
  int r = real_fmod_studio_create ? real_fmod_studio_create(system, flags) : 0;
  fprintf(stderr, "[FMODSPY] Studio::System::create(flags=0x%x) -> %d sys=%p\n",
          flags, r, system ? *system : NULL);
  return r;
}

static int parse_int_env(const char *name, int fallback) {
  const char *s = getenv(name);
  if (!s || !*s)
    return fallback;
  char *end = NULL;
  errno = 0;
  long v = strtol(s, &end, 0);
  if (errno || end == s)
    return fallback;
  return (int)v;
}

static int get_fmod_output_plugin(void *core_system, const char *want,
                                  unsigned *out_handle) {
  if (!real_fmod_system_get_num_plugins ||
      !real_fmod_system_get_plugin_handle ||
      !real_fmod_system_get_plugin_info)
    return 0;

  int count = 0;
  int nr = real_fmod_system_get_num_plugins(core_system, 0, &count);
  fprintf(stderr, "[FMODSPY] output plugins count -> %d n=%d\n", nr, count);
  if (nr != 0 || count <= 0)
    return 0;

  for (int i = 0; i < count && i < 64; i++) {
    unsigned handle = 0;
    int hr = real_fmod_system_get_plugin_handle(core_system, 0, i, &handle);
    int type = -1;
    unsigned version = 0;
    char name[128];
    memset(name, 0, sizeof(name));
    int ir = 0;
    if (hr == 0)
      ir = real_fmod_system_get_plugin_info(core_system, handle, &type, name,
                                            (int)sizeof(name), &version);
    fprintf(stderr,
            "[FMODSPY] output plugin[%d] handle=%u get=%d info=%d type=%d "
            "version=0x%x name=\"%s\"\n",
            i, handle, hr, ir, type, version, name);
    if (hr == 0 && ir == 0 && want && strstr(name, want)) {
      if (out_handle)
        *out_handle = handle;
      return 1;
    }
  }
  return 0;
}

static void force_fmod_output(void *studio_system) {
  if (!real_fmod_studio_get_core_system || !real_fmod_system_set_output)
    return;

  void *core_system = NULL;
  int gr = real_fmod_studio_get_core_system(studio_system, &core_system);
  fprintf(stderr, "[FMODSPY] Studio::System::getCoreSystem -> %d core=%p\n",
          gr, core_system);
  if (gr != 0 || !core_system)
    return;

  unsigned plugin_handle = 0;
  int forced_plugin = parse_int_env("BADLAND_FMOD_PLUGIN", -1);
  if (forced_plugin >= 0 && real_fmod_system_set_output_by_plugin) {
    int pr = real_fmod_system_set_output_by_plugin(core_system,
                                                   (unsigned)forced_plugin);
    fprintf(stderr, "[FMODSPY] force setOutputByPlugin(%d env) -> %d\n",
            forced_plugin, pr);
    if (pr == 0)
      return;
  }

  if (real_fmod_system_set_output_by_plugin &&
      get_fmod_output_plugin(core_system, "OpenSL", &plugin_handle)) {
    int pr = real_fmod_system_set_output_by_plugin(core_system, plugin_handle);
    fprintf(stderr,
            "[FMODSPY] force setOutputByPlugin(OpenSL handle=%u) -> %d\n",
            plugin_handle, pr);
    if (pr == 0)
      return;
  }

  int forced = parse_int_env("BADLAND_FMOD_OUTPUT", -1);
  if (forced >= 0) {
    int sr = real_fmod_system_set_output(core_system, forced);
    fprintf(stderr, "[FMODSPY] force setOutput(%d env) -> %d\n", forced, sr);
    return;
  }

  static const int outputs[] = {
      16, /* FMOD_OUTPUTTYPE_OPENSL in FMOD 1.08 headers */
      22, /* Some newer Android FMOD builds place OpenSL here */
      15, /* AudioTrack fallback for older FMOD */
      21, /* AudioTrack fallback for newer Android FMOD builds */
  };
  for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++) {
    int sr = real_fmod_system_set_output(core_system, outputs[i]);
    fprintf(stderr, "[FMODSPY] force setOutput(%d) -> %d\n", outputs[i], sr);
    if (sr == 0)
      return;
  }
}

static int spy_fmod_studio_initialize(void *system, int max_channels,
                                      unsigned flags, unsigned extra_flags,
                                      void *extra_driver_data) {
  force_fmod_output(system);
  int r = real_fmod_studio_initialize
              ? real_fmod_studio_initialize(system, max_channels, flags,
                                            extra_flags, extra_driver_data)
              : 0;
  fprintf(stderr,
          "[FMODSPY] Studio::System::initialize(sys=%p max=%d flags=0x%x "
          "extra=0x%x data=%p) -> %d\n",
          system, max_channels, flags, extra_flags, extra_driver_data, r);
  return r;
}

static const char *resolve_fmod_bank_path(const char *path, char *out,
                                          size_t out_size) {
  if (!path || !*path)
    return path;
  if (access(path, F_OK) == 0)
    return path;
  if (strncmp(path, "./assets/", 9) == 0 || strncmp(path, "assets/", 7) == 0)
    return path;
  if (snprintf(out, out_size, "./assets/%s", path) >= (int)out_size)
    return path;
  if (access(out, F_OK) == 0)
    return out;
  return path;
}

static int spy_fmod_studio_load_bank_file(void *system, const char *path,
                                          unsigned flags, void **bank) {
  char resolved[PATH_MAX];
  const char *use_path =
      resolve_fmod_bank_path(path, resolved, sizeof(resolved));
  int r = real_fmod_studio_load_bank_file
              ? real_fmod_studio_load_bank_file(system, use_path, flags, bank)
              : 0;
  fprintf(stderr,
          "[FMODSPY] Studio::System::loadBankFile(\"%s\" -> \"%s\", 0x%x) "
          "-> %d bank=%p\n",
          path ? path : "(null)", use_path ? use_path : "(null)", flags, r,
          bank ? *bank : NULL);
  return r;
}

static int spy_fmod_studio_get_core_system(void *system, void **core_system) {
  int r = real_fmod_studio_get_core_system
              ? real_fmod_studio_get_core_system(system, core_system)
              : 0;
  static unsigned log_count;
  if (log_count < 8 || (log_count % 120) == 0)
    fprintf(stderr,
            "[FMODSPY] Studio::System::getCoreSystem(sys=%p) -> %d core=%p\n",
            system, r, core_system ? *core_system : NULL);
  log_count++;
  return r;
}

static int spy_fmod_system_set_output(void *system, int output) {
  int r = real_fmod_system_set_output
              ? real_fmod_system_set_output(system, output)
              : 0;
  fprintf(stderr, "[FMODSPY] System::setOutput(sys=%p output=%d) -> %d\n",
          system, output, r);
  return r;
}

static int spy_fmod_system_set_dsp_buffer_size(void *system,
                                               unsigned buffer_length,
                                               int num_buffers) {
  int r = real_fmod_system_set_dsp_buffer_size
              ? real_fmod_system_set_dsp_buffer_size(system, buffer_length,
                                                     num_buffers)
              : 0;
  fprintf(stderr, "[FMODSPY] System::setDSPBufferSize(%u,%d) -> %d\n",
          buffer_length, num_buffers, r);
  return r;
}

static DynLibFunction fmod_spy_functions[] = {
    {"_ZN4FMOD6Studio6System6createEPPS1_j",
     (uintptr_t)&spy_fmod_studio_create},
    {"_ZN4FMOD6Studio6System10initializeEijjPv",
     (uintptr_t)&spy_fmod_studio_initialize},
    {"_ZN4FMOD6Studio6System12loadBankFileEPKcjPPNS0_4BankE",
     (uintptr_t)&spy_fmod_studio_load_bank_file},
    {"_ZNK4FMOD6Studio6System13getCoreSystemEPPNS_6SystemE",
     (uintptr_t)&spy_fmod_studio_get_core_system},
    {"_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE",
     (uintptr_t)&spy_fmod_system_set_output},
    {"_ZN4FMOD6System16setDSPBufferSizeEji",
     (uintptr_t)&spy_fmod_system_set_dsp_buffer_size},
};

static void init_fmod_spy_symbols(DynLibFunction *fmod_tbl, int fmod_n,
                                  DynLibFunction *studio_tbl, int studio_n) {
  DynLibFunction *f;
  f = so_find_import(studio_tbl, studio_n, "_ZN4FMOD6Studio6System6createEPPS1_j");
  if (f)
    real_fmod_studio_create = (void *)f->func;
  f = so_find_import(studio_tbl, studio_n,
                     "_ZN4FMOD6Studio6System10initializeEijjPv");
  if (f)
    real_fmod_studio_initialize = (void *)f->func;
  f = so_find_import(studio_tbl, studio_n,
                     "_ZN4FMOD6Studio6System12loadBankFileEPKcjPPNS0_4BankE");
  if (f)
    real_fmod_studio_load_bank_file = (void *)f->func;
  f = so_find_import(studio_tbl, studio_n,
                     "_ZNK4FMOD6Studio6System13getCoreSystemEPPNS_6SystemE");
  if (f)
    real_fmod_studio_get_core_system = (void *)f->func;
  f = so_find_import(fmod_tbl, fmod_n, "_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE");
  if (f)
    real_fmod_system_set_output = (void *)f->func;
  f = so_find_import(fmod_tbl, fmod_n, "_ZN4FMOD6System17setOutputByPluginEj");
  if (f)
    real_fmod_system_set_output_by_plugin = (void *)f->func;
  f = so_find_import(fmod_tbl, fmod_n,
                     "_ZN4FMOD6System13getNumPluginsE15FMOD_PLUGINTYPEPi");
  if (f)
    real_fmod_system_get_num_plugins = (void *)f->func;
  f = so_find_import(fmod_tbl, fmod_n,
                     "_ZN4FMOD6System15getPluginHandleE15FMOD_PLUGINTYPEiPj");
  if (f)
    real_fmod_system_get_plugin_handle = (void *)f->func;
  f = so_find_import(fmod_tbl, fmod_n,
                     "_ZN4FMOD6System13getPluginInfoEjP15FMOD_PLUGINTYPEPciPj");
  if (f)
    real_fmod_system_get_plugin_info = (void *)f->func;
  f = so_find_import(fmod_tbl, fmod_n, "_ZN4FMOD6System16setDSPBufferSizeEji");
  if (f)
    real_fmod_system_set_dsp_buffer_size = (void *)f->func;
  f = so_find_import(fmod_tbl, fmod_n, "FMOD_Android_JNI_Init");
  if (f)
    real_fmod_android_jni_init = (void *)f->func;
  fprintf(stderr,
          "[FMODSPY] real create=%p init=%p loadBank=%p getCore=%p "
          "setOutput=%p setOutputByPlugin=%p getNumPlugins=%p "
          "getPluginHandle=%p getPluginInfo=%p dsp=%p jniInit=%p\n",
          real_fmod_studio_create, real_fmod_studio_initialize,
          real_fmod_studio_load_bank_file, real_fmod_studio_get_core_system,
          real_fmod_system_set_output, real_fmod_system_set_output_by_plugin,
          real_fmod_system_get_num_plugins, real_fmod_system_get_plugin_handle,
          real_fmod_system_get_plugin_info,
          real_fmod_system_set_dsp_buffer_size, real_fmod_android_jni_init);
}

static void install_badland_audio_silence_hooks(void) {
  struct HookSpec {
    uintptr_t off;
    void *fn;
    const char *name;
  };
  static const struct HookSpec hooks[] = {
      {BADLAND_OFF_CONFIG_STOP_ALL_SOUNDS, badland_audio_void_silent,
       "Config::stopAllSounds"},
      {BADLAND_OFF_CONFIG_UPDATE_FMOD, badland_audio_void_silent,
       "Config::updateFMOD"},
      {BADLAND_OFF_CONFIG_SET_NORMAL_REVERB, badland_audio_void_silent,
       "Config::setNormalReverb"},
      {BADLAND_OFF_CONFIG_SET_TIMESCAPE_REVERB, badland_audio_void_silent,
       "Config::setTimeScapeReverb"},
      {BADLAND_OFF_CONFIG_IS_FMOD_READY, badland_audio_int_one,
       "Config::isFMODready"},
      {BADLAND_OFF_AUDIO_INITIALIZED, badland_audio_int_one,
       "Audio::initialized"},
      {BADLAND_OFF_EVENT_SOUND_SET_VISIBILITY, badland_audio_void_silent,
       "EventSound::setVisibility"},
      {BADLAND_OFF_EVENT_SOUND_SET_ACTIVE, badland_audio_void_silent,
       "EventSound::setActive"},
      {BADLAND_OFF_EVENT_SOUND_INIT_SOUND_EVENT, badland_audio_void_silent,
       "EventSound::initSoundEvent"},
      {BADLAND_OFF_EVENT_SOUND_PAUSE_SOUND, badland_audio_void_silent,
       "EventSound::pauseSound"},
      {BADLAND_OFF_AUDIO_GET_AUDIO_EVENT_DESC, badland_audio_desc_zero,
       "Audio::getAudioEventDesc"},
      {BADLAND_OFF_AUDIO_SUSPEND, badland_audio_void_silent, "Audio::suspend"},
      {BADLAND_OFF_AUDIO_RESUME, badland_audio_void_silent, "Audio::resume"},
      {BADLAND_OFF_AUDIO_PLAY_MUSIC, badland_audio_pair_silent,
       "Audio::playMusic"},
      {BADLAND_OFF_AUDIO_STOP_ALL_SOUNDS, badland_audio_void_silent,
       "Audio::stopAllSounds"},
      {BADLAND_OFF_AUDIO_SET_SOUND_POSITION, badland_audio_void_silent,
       "Audio::setSoundPosition"},
      {BADLAND_OFF_AUDIO_SET_GLOBAL_VOLUME, badland_audio_void_silent,
       "Audio::setGlobalVolume"},
      {BADLAND_OFF_AUDIO_PLAY_SOUND_CATEGORY, badland_audio_pair_silent,
       "Audio::playSoundCategory"},
      {BADLAND_OFF_AUDIO_PLAY_SOUND_IMPL, badland_audio_pair_silent,
       "Audio::playSoundImpl"},
      {BADLAND_OFF_AUDIO_INIT_AUDIO_EVENT, badland_audio_pair_silent,
       "Audio::initAudioEvent"},
      {BADLAND_OFF_AUDIO_PLAY_SOUND, badland_audio_pair_silent,
       "Audio::playSound"},
      {BADLAND_OFF_AUDIO_PLAY_SOUND_CUSTOM, badland_audio_pair_silent,
       "Audio::playSoundWithCustomParameter"},
      {BADLAND_OFF_AUDIO_STOP_MUSIC, badland_audio_void_silent,
       "Audio::stopMusic"},
      {BADLAND_OFF_AUDIO_PLAY_AMBIENT, badland_audio_pair_silent,
       "Audio::playAmbient"},
      {BADLAND_OFF_AUDIO_UNINIT_BANK, badland_audio_void_silent,
       "Audio::uninitBank"},
      {BADLAND_OFF_AUDIO_SET_LISTENER_POSITION, badland_audio_void_silent,
       "Audio::setListenerPosition"},
      {BADLAND_OFF_AUDIO_UPDATE, badland_audio_void_silent, "Audio::update"},
      {BADLAND_OFF_AUDIO_UNINIT, badland_audio_void_silent, "Audio::uninit"},
  };

  fprintf(stderr, "[audio] FMOD off: installing silent sky::Audio hooks\n");
  for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++)
    patch_game_branch_abs(hooks[i].off, hooks[i].fn, hooks[i].name);
}

static void install_badland_pvr_hooks(void) {
  real_pvr_v2 =
      (void *)so_find_addr_safe("_ZN7cocos2d12CCTexturePVR15unpackPVRv2DataEPhj");
  real_pvr_v3 =
      (void *)so_find_addr_safe("_ZN7cocos2d12CCTexturePVR15unpackPVRv3DataEPhj");
  real_pvr_create_gl =
      (void *)so_find_addr_safe("_ZN7cocos2d12CCTexturePVR15createGLTextureEv");
  if (real_pvr_v2)
    g_game_base = (uintptr_t)real_pvr_v2 - BADLAND_OFF_PVR_V2;
  else
    g_game_base = (uintptr_t)text_base;
  fprintf(stderr, "[PVR] base=%p v2=%p v3=%p createGL=%p\n",
          (void *)g_game_base, real_pvr_v2, real_pvr_v3, real_pvr_create_gl);
  patch_game_got(BADLAND_GOT_PVR_V2, badland_pvr_v2, "unpackPVRv2");
  patch_game_got(BADLAND_GOT_PVR_V3, badland_pvr_v3, "unpackPVRv3");
  patch_game_got(BADLAND_GOT_PVR_CREATE_GL, badland_pvr_create_gl,
                 "createGLTexture");
  game_new_array = (void *)so_find_addr_safe("_Znam");
  real_ccfile_get_data = (void *)patch_game_ptr(
      BADLAND_VT_CCFILEUTILSANDROID_GETFILEDATA, badland_ccfile_get_data,
      "CCFileUtilsAndroid::getFileData");
  if (!g_enable_fmod)
    install_badland_audio_silence_hooks();
}

static void crash_handler(int sig, siginfo_t *si, void *arg) {
  ucontext_t *uc = (ucontext_t *)arg;
#if defined(__aarch64__)
  uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
  uintptr_t lr = (uintptr_t)uc->uc_mcontext.regs[30];
  uintptr_t sp = (uintptr_t)uc->uc_mcontext.sp;
  uintptr_t pstate = (uintptr_t)uc->uc_mcontext.pstate;
#else
  uintptr_t pc = 0;
  uintptr_t lr = 0;
  uintptr_t sp = 0;
  uintptr_t pstate = 0;
#endif
  uintptr_t base = (uintptr_t)text_base;
  fprintf(stderr,
          "CRASH sig=%d addr=%p pc=%p lr=%p libbadland+0x%lx lr+0x%lx\n",
          sig, si ? si->si_addr : NULL, (void *)pc, (void *)lr,
          base ? (unsigned long)(pc - base) : 0,
          base ? (unsigned long)(lr - base) : 0);
  fprintf(stderr, "CRASH sp=%p pstate=0x%lx text=%p+%zu\n", (void *)sp,
          (unsigned long)pstate, text_base, text_size);
#if defined(__aarch64__)
  for (int i = 0; i < 28; i += 4) {
    fprintf(stderr,
            "CRASH x%02d=%p x%02d=%p x%02d=%p x%02d=%p\n", i,
            (void *)(uintptr_t)uc->uc_mcontext.regs[i], i + 1,
            (void *)(uintptr_t)uc->uc_mcontext.regs[i + 1], i + 2,
            (void *)(uintptr_t)uc->uc_mcontext.regs[i + 2], i + 3,
            (void *)(uintptr_t)uc->uc_mcontext.regs[i + 3]);
  }
  fprintf(stderr, "CRASH x28=%p x29=%p x30=%p\n",
          (void *)(uintptr_t)uc->uc_mcontext.regs[28],
          (void *)(uintptr_t)uc->uc_mcontext.regs[29],
          (void *)(uintptr_t)uc->uc_mcontext.regs[30]);
#endif
  FILE *maps = fopen("/proc/self/maps", "r");
  if (maps) {
    char line[512];
    while (fgets(line, sizeof(line), maps)) {
      unsigned long start = 0, end = 0;
      if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
        if ((pc >= start && pc < end) || (lr >= start && lr < end))
          fprintf(stderr, "CRASH map: %s", line);
      }
    }
    fclose(maps);
  }
  if (base && text_size && sp) {
    uintptr_t *stack = (uintptr_t *)sp;
    int found = 0;
    for (size_t i = 0; i < 512 && found < 24; i++) {
      uintptr_t v = stack[i];
      if (v >= base && v < base + text_size) {
        fprintf(stderr, "CRASH stack[%zu]=%p libbadland+0x%lx\n", i,
                (void *)v, (unsigned long)(v - base));
        found++;
      }
    }
  }
#if defined(__aarch64__)
  if (base && text_size) {
    uintptr_t fp = (uintptr_t)uc->uc_mcontext.regs[29];
    for (int depth = 0; depth < 32 && fp; depth++) {
      if (fp < sp || fp - sp > (8u * 1024u * 1024u))
        break;
      uintptr_t next_fp = ((uintptr_t *)fp)[0];
      uintptr_t ret = ((uintptr_t *)fp)[1];
      if (ret >= base && ret < base + text_size)
        fprintf(stderr, "CRASH fp[%d]=%p ret=%p libbadland+0x%lx\n", depth,
                (void *)fp, (void *)ret, (unsigned long)(ret - base));
      if (next_fp <= fp || next_fp - fp > (1024u * 1024u))
        break;
      fp = next_fp;
    }
  }
#endif
  _Exit(128 + sig);
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

static DynLibFunction *combine_tables(int *out_n, DynLibFunction *a, int an,
                                      DynLibFunction *b, int bn,
                                      DynLibFunction *c, int cn) {
  int total = an + bn + cn;
  DynLibFunction *out = calloc((size_t)total, sizeof(*out));
  int k = 0;
  if (a && an) {
    memcpy(out + k, a, sizeof(*out) * (size_t)an);
    k += an;
  }
  if (b && bn) {
    memcpy(out + k, b, sizeof(*out) * (size_t)bn);
    k += bn;
  }
  if (c && cn) {
    memcpy(out + k, c, sizeof(*out) * (size_t)cn);
    k += cn;
  }
  *out_n = k;
  return out;
}

static void load_module(const char *name, int heap_mb, DynLibFunction *tbl,
                        int n) {
  size_t heap_size = (size_t)heap_mb * 1024 * 1024;
  void *heap = mmap(NULL, heap_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED)
    fatal_error("mmap %s %dMB: %s", name, heap_mb, strerror(errno));

  fprintf(stderr, "== carregando %s (%d MB) ==\n", name, heap_mb);
  if (so_load(name, heap, heap_size) < 0)
    fatal_error("so_load(%s)", name);
  if (so_relocate() < 0)
    fatal_error("so_relocate(%s)", name);
  so_resolve(tbl, n, 0);
  so_finalize();
  so_flush_caches();
  so_execute_init_array();
  fprintf(stderr, "== %s: text=%p+%zu ==\n", name, text_base, text_size);
}

static void mkdir_p(const char *path) {
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      mkdir(tmp, 0775);
      *p = '/';
    }
  }
  mkdir(tmp, 0775);
}

static void path_join(char *out, size_t n, const char *a, const char *b) {
  snprintf(out, n, "%s/%s", a, b);
}

static void watchdog_touch(unsigned frame) {
  if (!g_watchdog_path[0])
    return;
  time_t now = time(NULL);
  if (frame && now == g_watchdog_last_write)
    return;
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s.tmp", g_watchdog_path);
  FILE *f = fopen(tmp, "w");
  if (!f)
    return;
  fprintf(f, "%ld %u\n", (long)now, frame);
  fclose(f);
  rename(tmp, g_watchdog_path);
  g_watchdog_last_write = now;
}

static int init_sdl(SDL_Window **out_window, SDL_GLContext *out_context) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK |
               SDL_INIT_GAMECONTROLLER) != 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 0;
  }
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    g_width = dm.w;
    g_height = dm.h;
  }

  SDL_Window *window = SDL_CreateWindow(
      "BADLAND", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, g_width,
      g_height, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN);
  if (!window) {
    fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    return 0;
  }
  SDL_GLContext ctx = SDL_GL_CreateContext(window);
  if (!ctx) {
    fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
    return 0;
  }
  SDL_GL_MakeCurrent(window, ctx);
  SDL_GL_SetSwapInterval(1);
  SDL_GL_GetDrawableSize(window, &g_width, &g_height);
  if (g_width <= 0)
    g_width = 1280;
  if (g_height <= 0)
    g_height = 720;

  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (SDL_IsGameController(i)) {
      g_controller = SDL_GameControllerOpen(i);
      if (g_controller) {
        fprintf(stderr, "controller: %s\n", SDL_GameControllerName(g_controller));
        break;
      }
    }
  }

  *out_window = window;
  *out_context = ctx;
  fprintf(stderr, "SDL/GLES2 pronto: %dx%d\n", g_width, g_height);
  return 1;
}

static int map_btn_android(int b) {
  switch (b) {
  case SDL_CONTROLLER_BUTTON_A:
    return AKEYCODE_BUTTON_A;
  case SDL_CONTROLLER_BUTTON_B:
    return AKEYCODE_BUTTON_B;
  case SDL_CONTROLLER_BUTTON_X:
    return AKEYCODE_BUTTON_X;
  case SDL_CONTROLLER_BUTTON_Y:
    return AKEYCODE_BUTTON_Y;
  case SDL_CONTROLLER_BUTTON_START:
    return AKEYCODE_BUTTON_START;
  case SDL_CONTROLLER_BUTTON_BACK:
    return AKEYCODE_BUTTON_SELECT;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
    return AKEYCODE_BUTTON_L1;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
    return AKEYCODE_BUTTON_R1;
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

static int map_btn_pad(int b) {
  switch (b) {
  case SDL_CONTROLLER_BUTTON_DPAD_UP:
    return BADLAND_PAD_UP;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
    return BADLAND_PAD_DOWN;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
    return BADLAND_PAD_LEFT;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
    return BADLAND_PAD_RIGHT;
  case SDL_CONTROLLER_BUTTON_A:
    return BADLAND_PAD_A;
  case SDL_CONTROLLER_BUTTON_B:
    return BADLAND_PAD_B;
  case SDL_CONTROLLER_BUTTON_X:
    return BADLAND_PAD_X;
  case SDL_CONTROLLER_BUTTON_Y:
    return BADLAND_PAD_Y;
  case SDL_CONTROLLER_BUTTON_START:
  case SDL_CONTROLLER_BUTTON_BACK:
    return BADLAND_PAD_MENU;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
    return BADLAND_PAD_L1;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
    return BADLAND_PAD_R1;
  default:
    return -1;
  }
}

static int map_key_android(SDL_Keycode k) {
  switch (k) {
  case SDLK_UP:
    return AKEYCODE_DPAD_UP;
  case SDLK_DOWN:
    return AKEYCODE_DPAD_DOWN;
  case SDLK_LEFT:
    return AKEYCODE_DPAD_LEFT;
  case SDLK_RIGHT:
    return AKEYCODE_DPAD_RIGHT;
  case SDLK_SPACE:
  case SDLK_z:
    return AKEYCODE_BUTTON_A;
  case SDLK_x:
    return AKEYCODE_BUTTON_B;
  case SDLK_a:
    return AKEYCODE_BUTTON_X;
  case SDLK_s:
    return AKEYCODE_BUTTON_Y;
  case SDLK_RETURN:
    return AKEYCODE_BUTTON_START;
  case SDLK_BACKSPACE:
    return AKEYCODE_BUTTON_SELECT;
  case SDLK_ESCAPE:
    return AKEYCODE_BACK;
  default:
    return -1;
  }
}

static int map_key_pad(SDL_Keycode k) {
  switch (k) {
  case SDLK_UP:
    return BADLAND_PAD_UP;
  case SDLK_DOWN:
    return BADLAND_PAD_DOWN;
  case SDLK_LEFT:
    return BADLAND_PAD_LEFT;
  case SDLK_RIGHT:
    return BADLAND_PAD_RIGHT;
  case SDLK_SPACE:
  case SDLK_z:
  case SDLK_RETURN:
    return BADLAND_PAD_A;
  case SDLK_x:
    return BADLAND_PAD_B;
  case SDLK_a:
    return BADLAND_PAD_X;
  case SDLK_s:
    return BADLAND_PAD_Y;
  case SDLK_BACKSPACE:
    return BADLAND_PAD_MENU;
  default:
    return -1;
  }
}

static void send_touch(int down) {
  float x = g_width * 0.5f;
  float y = g_height * 0.5f;
  if (down && !g_touch_down && nativeTouchesBegin) {
    nativeTouchesBegin(g_env, NULL, 0, x, y);
    g_touch_down = 1;
  } else if (!down && g_touch_down && nativeTouchesEnd) {
    nativeTouchesEnd(g_env, NULL, 0, x, y);
    g_touch_down = 0;
  }
}

static void send_pad_action(int action, int button, float x, float y) {
  if (!nativePadAction || button < 0)
    return;
  if (g_input_log) {
    fprintf(stderr, "[INPUT] pad action=%d button=%d x=%.3f y=%.3f\n", action,
            button, x, y);
  }
  nativePadAction(g_env, NULL, 0, action, button, x, y);
}

static void send_button(int sdl_button, int down) {
  int button = map_btn_pad(sdl_button);
  send_pad_action(down ? BADLAND_PAD_ACTION_DOWN : BADLAND_PAD_ACTION_UP,
                  button, 0.0f, 0.0f);
}

static float axis_to_float(Sint16 value) {
  const int deadzone = 8000;
  if (value > -deadzone && value < deadzone)
    return 0.0f;
  if (value >= 32767)
    return 1.0f;
  if (value <= -32768)
    return -1.0f;
  return (float)value / 32767.0f;
}

static float trigger_to_float(Sint16 value) {
  float v = ((float)value + 32768.0f) / 65535.0f;
  if (v < 0.08f)
    return 0.0f;
  if (v > 1.0f)
    return 1.0f;
  return v;
}

static void input_selftest_tick(unsigned frame) {
  switch (frame) {
  case 150:
  case 300:
  case 450:
  case 650:
  case 800:
  case 950:
  case 1100:
    fprintf(stderr, "[INPUT] selftest A down frame=%u\n", frame);
    send_pad_action(BADLAND_PAD_ACTION_DOWN, BADLAND_PAD_A, 0.0f, 0.0f);
    break;
  case 158:
  case 308:
  case 458:
  case 658:
  case 808:
  case 958:
  case 1108:
    fprintf(stderr, "[INPUT] selftest A up frame=%u\n", frame);
    send_pad_action(BADLAND_PAD_ACTION_UP, BADLAND_PAD_A, 0.0f, 0.0f);
    break;
  default:
    break;
  }
}

extern unsigned long badland_gl_draw_calls_total(void);
extern const char *badland_gl_current_texture_label(void);
extern unsigned badland_gl_current_texture_id(void);
extern unsigned badland_gl_current_program_id(void);
extern unsigned badland_gl_current_blend_src(void);
extern unsigned badland_gl_current_blend_dst(void);

static void log_frame_pixels(unsigned frame) {
  if (frame != 1 && frame != 5 && frame != 30 && frame != 120 &&
      frame != 300 && (frame % 600) != 0)
    return;

  unsigned long long sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0;
  unsigned min_r = 255, min_g = 255, min_b = 255, min_a = 255;
  unsigned max_r = 0, max_g = 0, max_b = 0, max_a = 0;
  unsigned char px[4];
  int samples = 0;

  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  for (int y = 0; y < 9; y++) {
    int py = (int)(((long long)(y * 2 + 1) * g_height) / 18);
    if (py >= g_height)
      py = g_height - 1;
    for (int x = 0; x < 16; x++) {
      int pxpos = (int)(((long long)(x * 2 + 1) * g_width) / 32);
      if (pxpos >= g_width)
        pxpos = g_width - 1;
      memset(px, 0, sizeof(px));
      glReadPixels(pxpos, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
      unsigned r = px[0], g = px[1], b = px[2], a = px[3];
      sum_r += r;
      sum_g += g;
      sum_b += b;
      sum_a += a;
      if (r < min_r)
        min_r = r;
      if (g < min_g)
        min_g = g;
      if (b < min_b)
        min_b = b;
      if (a < min_a)
        min_a = a;
      if (r > max_r)
        max_r = r;
      if (g > max_g)
        max_g = g;
      if (b > max_b)
        max_b = b;
      if (a > max_a)
        max_a = a;
      samples++;
    }
  }

  GLenum err = glGetError();
  if (samples <= 0)
    return;
  static unsigned long last_draw_total;
  unsigned long draw_total = badland_gl_draw_calls_total();
  unsigned long draw_delta = draw_total - last_draw_total;
  last_draw_total = draw_total;
  const char *tex_label = badland_gl_current_texture_label();
  fprintf(stderr,
          "[framepix] frame=%u samples=%d avg=%llu,%llu,%llu,%llu "
          "min=%u,%u,%u,%u max=%u,%u,%u,%u draws=%lu(+%lu) tex=%u "
          "prog=%u blend=0x%x/0x%x label=\"%s\" glerr=0x%x\n",
          frame, samples, sum_r / samples, sum_g / samples, sum_b / samples,
          sum_a / samples, min_r, min_g, min_b, min_a, max_r, max_g, max_b,
          max_a, draw_total, draw_delta, badland_gl_current_texture_id(),
          badland_gl_current_program_id(), badland_gl_current_blend_src(),
          badland_gl_current_blend_dst(), tex_label ? tex_label : "",
          (unsigned)err);
}

static void dump_framebuffer(unsigned frame) {
  if (frame != 300 &&
      !(g_input_selftest &&
        (frame == 600 || frame == 900 || frame == 1200 || frame == 1500)))
    return;
  const char *env = getenv("BADLAND_FRAMEDUMP");
  if (env && strcmp(env, "0") == 0)
    return;
  int w = g_width;
  int h = g_height;
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
    return;
  size_t bytes = (size_t)w * h * 4u;
  unsigned char *rgba = malloc(bytes);
  if (!rgba)
    return;
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "./logs/frame-%u.ppm", frame);
  FILE *f = fopen(path, "wb");
  if (f) {
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--) {
      unsigned char *row = rgba + (size_t)y * w * 4u;
      for (int x = 0; x < w; x++)
        fwrite(row + x * 4, 1, 3, f);
    }
    fclose(f);
    fprintf(stderr, "[framepix] dumped %s\n", path);
  }
  free(rgba);
}

int main(int argc, char **argv) {
  volatile char c = g_bionic_guard_pad[0];
  (void)c;
  install_crash_handler();

  char gamedir[PATH_MAX];
  if (argc > 1)
    snprintf(gamedir, sizeof(gamedir), "%s", argv[1]);
  else if (!getcwd(gamedir, sizeof(gamedir)))
    snprintf(gamedir, sizeof(gamedir), ".");
  chdir(gamedir);

  char apk_path[PATH_MAX];
  char logs_path[PATH_MAX];
  path_join(apk_path, sizeof(apk_path), gamedir, "game.apk");
  path_join(logs_path, sizeof(logs_path), gamedir, "logs");
  mkdir_p(logs_path);
  const char *hb_env = getenv("BADLAND_WATCHDOG_HEARTBEAT");
  if (hb_env && hb_env[0])
    snprintf(g_watchdog_path, sizeof(g_watchdog_path), "%s", hb_env);
  else
    path_join(g_watchdog_path, sizeof(g_watchdog_path), logs_path, "heartbeat");
  watchdog_touch(0);

  fprintf(stderr, "=== BADLAND Cocos/FMOD so-loader ===\n");
  fprintf(stderr, "gamedir=%s\napk=%s\n", gamedir, apk_path);
  const char *enable_fmod_env = getenv("BADLAND_ENABLE_FMOD");
  g_enable_fmod = enable_fmod_env && strcmp(enable_fmod_env, "0") != 0;
  const char *enable_resume_env = getenv("BADLAND_ENABLE_RESUME");
  g_enable_resume = enable_resume_env && strcmp(enable_resume_env, "0") != 0;
  g_input_log = parse_int_env("BADLAND_INPUTLOG", 0) != 0;
  g_input_selftest = parse_int_env("BADLAND_INPUT_SELFTEST", 0) != 0;
  fprintf(stderr, "native FMOD init: %s\n", g_enable_fmod ? "on" : "off");
  fprintf(stderr, "nativeOnResume: %s\n", g_enable_resume ? "on" : "off");
  fprintf(stderr, "input log: %s\n", g_input_log ? "on" : "off");
  fprintf(stderr, "input selftest: %s\n", g_input_selftest ? "on" : "off");

  load_module(FMOD_SO, FMOD_HEAP_MB, dynlib_functions, dynlib_functions_count);
  fmod_JNI_OnLoad_fn = (void *)so_find_addr_safe("JNI_OnLoad");
  int fmod_n = 0;
  DynLibFunction *fmod_tbl = so_snapshot_symbols(&fmod_n);
  fprintf(stderr, "fmod exports: %d\n", fmod_n);

  int studio_comb_n = 0;
  DynLibFunction *studio_comb = combine_tables(
      &studio_comb_n, dynlib_functions, dynlib_functions_count, fmod_tbl,
      fmod_n, NULL, 0);
  load_module(FMODSTUDIO_SO, FMODSTUDIO_HEAP_MB, studio_comb, studio_comb_n);
  int studio_n = 0;
  DynLibFunction *studio_tbl = so_snapshot_symbols(&studio_n);
  fprintf(stderr, "fmodstudio exports: %d\n", studio_n);
  init_fmod_spy_symbols(fmod_tbl, fmod_n, studio_tbl, studio_n);

  int base_comb_n = 0;
  DynLibFunction *base_comb = combine_tables(
      &base_comb_n, fmod_spy_functions,
      (int)(sizeof(fmod_spy_functions) / sizeof(fmod_spy_functions[0])),
      dynlib_functions, dynlib_functions_count, NULL, 0);
  int game_comb_n = 0;
  DynLibFunction *game_comb = combine_tables(
      &game_comb_n, base_comb, base_comb_n, fmod_tbl, fmod_n, studio_tbl,
      studio_n);
  load_module(GAME_SO, GAME_HEAP_MB, game_comb, game_comb_n);
  install_badland_pvr_hooks();

  JNI_OnLoad_fn = (void *)so_find_addr_safe("JNI_OnLoad");
  nativeSetApkPath =
      (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetApkPath");
  nativeInit =
      (void *)so_find_addr("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit");
  nativeRender =
      (void *)so_find_addr("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender");
  nativeOnStart =
      (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnStart");
  nativeOnStop =
      (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnStop");
  nativeOnPause =
      (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause");
  nativeOnResume =
      (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume");
  nativeKeyDown =
      (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyDown");
  nativePadAction =
      (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativePadAction");
  nativeTouchesBegin =
      (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin");
  nativeTouchesEnd =
      (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd");
  nativeTouchesMove =
      (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesMove");
  nativeInitializeAssetManager = (void *)so_find_addr_safe(
      "Java_com_frogmind_badland_Badland_nativeInitializeAssetManager");
  nativeInitFmod =
      (void *)so_find_addr_safe("Java_com_frogmind_badland_Badland_nativeInitFmod");
  nativeReportAudioProperties = (void *)so_find_addr_safe(
      "Java_com_frogmind_badland_Badland_nativeReportAudioProperties");
  nativeSetPreferredFmodOutput = (void *)so_find_addr_safe(
      "Java_com_frogmind_badland_Badland_nativeSetPreferredFmodOutput");
  nativeSetForceAudioTrackOutput = (void *)so_find_addr_safe(
      "Java_com_frogmind_badland_Badland_nativeSetForceAudioTrackOutput");
  nativeSetAudioPaused = (void *)so_find_addr_safe(
      "Java_com_frogmind_badland_Badland_nativeSetAudioPaused");

  void *fake_vm = NULL;
  void *fake_env = NULL;
  jni_shim_init(&fake_vm, &fake_env);
  g_env = fake_env;
  jni_set_writable_path("./userdata/");
  mkdir_p("./userdata");

  if (g_enable_fmod && real_fmod_android_jni_init) {
    int r = real_fmod_android_jni_init(fake_vm, (void *)0x1337);
    fprintf(stderr, "[FMODSPY] FMOD_Android_JNI_Init -> %d\n", r);
  }
  if (g_enable_fmod && fmod_JNI_OnLoad_fn) {
    jint r = fmod_JNI_OnLoad_fn(fake_vm, NULL);
    fprintf(stderr, "[FMODSPY] FMOD JNI_OnLoad -> 0x%x\n", (unsigned)r);
  }

  if (JNI_OnLoad_fn) {
    fprintf(stderr, "JNI_OnLoad\n");
    JNI_OnLoad_fn(fake_vm, NULL);
  }

  void *j_apk = jni_make_string(apk_path);
  if (nativeSetApkPath) {
    fprintf(stderr, "nativeSetApkPath\n");
    nativeSetApkPath(g_env, NULL, j_apk);
  }
  if (nativeInitializeAssetManager) {
    fprintf(stderr, "nativeInitializeAssetManager\n");
    nativeInitializeAssetManager(g_env, NULL, (void *)0x1337);
  }
  if (g_enable_fmod) {
    if (nativeReportAudioProperties)
      nativeReportAudioProperties(g_env, NULL, 48000, 1024);
    if (nativeSetForceAudioTrackOutput)
      nativeSetForceAudioTrackOutput(g_env, NULL, 0);
  }

  SDL_Window *window = NULL;
  SDL_GLContext glctx = NULL;
  if (!init_sdl(&window, &glctx))
    return 1;

  int gles_version = parse_int_env("BADLAND_GLES_VERSION", 2);
  if (gles_version < 1 || gles_version > 3)
    gles_version = 2;
  fprintf(stderr, "nativeInit(gles=%d,width=%d,height=%d)\n", gles_version,
          g_width, g_height);
  nativeInit(g_env, NULL, gles_version, g_width, g_height);
  fprintf(stderr, "nativeInit done\n");
  if (g_enable_fmod && nativeInitFmod) {
    fprintf(stderr, "nativeInitFmod(0)\n");
    nativeInitFmod(g_env, NULL, 0);
    fprintf(stderr, "nativeInitFmod done\n");
  }
  if (nativeOnStart) {
    fprintf(stderr, "nativeOnStart\n");
    nativeOnStart(g_env, NULL);
    fprintf(stderr, "nativeOnStart done\n");
  }
  if (g_enable_resume && nativeOnResume) {
    fprintf(stderr, "nativeOnResume\n");
    nativeOnResume(g_env, NULL);
    fprintf(stderr, "nativeOnResume done\n");
  }
  if (g_enable_fmod && nativeSetAudioPaused)
    nativeSetAudioPaused(g_env, NULL, 0);

  int running = 1;
  int back_down = 0;
  int start_down = 0;
  float left_x = 0.0f;
  float left_y = 0.0f;
  unsigned frame = 0;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT)
        running = 0;
      else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
        int down = e.type == SDL_KEYDOWN;
        if (e.key.repeat)
          continue;
        int key = map_key_android(e.key.keysym.sym);
        int button = map_key_pad(e.key.keysym.sym);
        if (key == AKEYCODE_BACK && down) {
          running = 0;
          continue;
        }
        if (button >= 0)
          send_pad_action(down ? BADLAND_PAD_ACTION_DOWN
                               : BADLAND_PAD_ACTION_UP,
                          button, 0.0f, 0.0f);
      } else if (e.type == SDL_CONTROLLERBUTTONDOWN ||
                 e.type == SDL_CONTROLLERBUTTONUP) {
        int down = e.type == SDL_CONTROLLERBUTTONDOWN;
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)
          back_down = down;
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_START)
          start_down = down;
        if (back_down && start_down) {
          running = 0;
          break;
        }
        send_button(e.cbutton.button, down);
      } else if (e.type == SDL_CONTROLLERAXISMOTION) {
        if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX ||
            e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
          if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX)
            left_x = axis_to_float(e.caxis.value);
          else
            left_y = axis_to_float(e.caxis.value);
          send_pad_action(BADLAND_PAD_ACTION_ANALOG, BADLAND_PAD_LEFT_ANALOG,
                          left_x, left_y);
        } else if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) {
          send_pad_action(BADLAND_PAD_ACTION_ANALOG, BADLAND_PAD_L2,
                          trigger_to_float(e.caxis.value), 0.0f);
        } else if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
          send_pad_action(BADLAND_PAD_ACTION_ANALOG, BADLAND_PAD_R2,
                          trigger_to_float(e.caxis.value), 0.0f);
        }
      }
    }

    if (g_input_selftest)
      input_selftest_tick(frame + 1);

    if (frame < 5)
      fprintf(stderr, "nativeRender frame %u\n", frame + 1);
    nativeRender(g_env, NULL);
    if (frame < 5)
      fprintf(stderr, "nativeRender frame %u done\n", frame + 1);
    log_frame_pixels(frame + 1);
    dump_framebuffer(frame + 1);
    SDL_GL_SwapWindow(window);
    watchdog_touch(++frame);
  }

  fprintf(stderr, "saindo\n");
  if (g_enable_fmod && nativeSetAudioPaused)
    nativeSetAudioPaused(g_env, NULL, 1);
  if (g_enable_resume && nativeOnPause)
    nativeOnPause(g_env, NULL);
  if (nativeOnStop)
    nativeOnStop(g_env, NULL);
  if (g_controller)
    SDL_GameControllerClose(g_controller);
  SDL_GL_DeleteContext(glctx);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
